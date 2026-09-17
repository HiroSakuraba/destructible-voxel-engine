#include "dve/chiptune_vertical_slice.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <type_traits>
#include <utility>

namespace dve::gameplay {
namespace {

constexpr float kPlayerRunSpeed = 118.0F;
constexpr float kGroundAcceleration = 860.0F;
constexpr float kAirAcceleration = 460.0F;
constexpr float kGroundDeceleration = 1100.0F;
constexpr float kGravity = 880.0F;
constexpr float kJumpSpeed = 315.0F;
constexpr float kMaximumFallSpeed = 620.0F;
constexpr float kEnemySpeed = 34.0F;
constexpr float kProjectileSpeed = 290.0F;
constexpr float kDamageCooldown = 0.65F;
constexpr float kCoyoteWindow = 0.10F;
constexpr float kJumpBufferWindow = 0.12F;

[[nodiscard]] float move_towards(float current, float target, float maximumDelta) noexcept {
    if (current < target) return std::min(current + maximumDelta, target);
    if (current > target) return std::max(current - maximumDelta, target);
    return target;
}

void hash_byte(std::uint64_t& state, std::uint8_t value) noexcept {
    state ^= value;
    state *= 1099511628211ULL;
}

template <typename T>
void hash_scalar(std::uint64_t& state, T value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto bytes = std::bit_cast<std::array<std::byte, sizeof(T)>>(value);
    for (const auto byte : bytes) hash_byte(state, std::to_integer<std::uint8_t>(byte));
}

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "could not open " + path.string();
        return {};
    }
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (!input.good() && !input.eof()) {
        if (error) *error = "could not read " + path.string();
        return {};
    }
    return text;
}

[[nodiscard]] TileAabb object_bounds(const TileObject& object) noexcept {
    const float width = object.size.x > 0.0F ? object.size.x : 12.0F;
    const float height = object.size.y > 0.0F ? object.size.y : 12.0F;
    return {{object.position.x - (object.size.x > 0.0F ? 0.0F : width * 0.5F),
             object.position.y - (object.size.y > 0.0F ? 0.0F : height * 0.5F)},
            {width, height}};
}

[[nodiscard]] audio::ChipSfxRequest cue_request(std::size_t index) noexcept {
    audio::ChipSfxRequest request;
    request.sampleRate = 48000U;
    request.gain = 0.58F;
    request.durationSeconds = 0.24F;
    switch (index) {
        case 0U: request.preset = audio::ChipSfxPreset::Jump; request.baseMidi = 64; break;
        case 1U: request.preset = audio::ChipSfxPreset::Laser; request.baseMidi = 76; request.pan = 0.12F; break;
        case 2U: request.preset = audio::ChipSfxPreset::Hit; request.baseMidi = 54; break;
        case 3U: request.preset = audio::ChipSfxPreset::Coin; request.baseMidi = 84; request.gain = 0.50F; break;
        case 4U: request.preset = audio::ChipSfxPreset::PowerUp; request.baseMidi = 72; request.durationSeconds = 0.38F; break;
        case 5U: request.preset = audio::ChipSfxPreset::UiCancel; request.baseMidi = 45; break;
        default: request.preset = audio::ChipSfxPreset::Explosion; request.baseMidi = 42; request.gain = 0.45F; break;
    }
    return request;
}

} // namespace

ChiptuneVerticalSlice::ChiptuneVerticalSlice(audio::AudioMixer& mixer) : mixer_(mixer) {
    hud_.set_tools({{"pulse_shot", "Pulse Shot", true}});
}

bool ChiptuneVerticalSlice::initialize(const std::filesystem::path& projectRoot,
                                       ChiptuneSliceAssets assets,
                                       std::string* error) {
    assetPaths_ = std::move(assets);
    const auto levelPath = projectRoot / assetPaths_.level;
    const auto musicPath = projectRoot / assetPaths_.music;
    if (!load_level(levelPath, error)) return false;
    if (!load_audio(musicPath, error)) return false;
    if (!register_cues(error)) return false;
    reset_world_state();
    initialized_ = true;
    emit(ChiptuneSliceEventType::MusicStarted, assetPaths_.music.generic_string());
    return true;
}

bool ChiptuneVerticalSlice::load_level(const std::filesystem::path& path, std::string* error) {
    const std::string text = read_text_file(path, error);
    if (text.empty()) return false;
    TileMap candidate;
    if (!TileMap::parse(text, candidate, error)) return false;
    map_ = std::move(candidate);
    collisionGrid_ = build_collision_grid(map_);
    return true;
}

bool ChiptuneVerticalSlice::load_audio(const std::filesystem::path& path, std::string* error) {
    const std::string text = read_text_file(path, error);
    if (text.empty()) return false;
    audio::ChipSong song;
    if (!audio::ChipSong::parse(text, song, error)) return false;
    auto rendered = audio::render_chiptune_audio_asset(song, 12.0F, error);
    if (rendered.samples.empty()) return false;
    audio::ResidentSampleDesc resident;
    resident.name = song.name + " [vertical slice music]";
    resident.sampleRate = rendered.metadata.sampleRate;
    resident.channels = rendered.metadata.channels;
    resident.samples = std::move(rendered.samples);
    musicSample_ = mixer_.register_resident_sample(std::move(resident), error);
    if (!musicSample_) return false;
    audio::PlaySampleDesc play;
    play.sample = musicSample_;
    play.bus = audio::AudioBusId::Music;
    play.priority = audio::AudioPriority::Hero;
    play.loop = true;
    play.spatialized = false;
    play.gain = 0.72F;
    musicSource_ = mixer_.play_sample(play);
    if (!musicSource_) {
        if (error) *error = "could not start vertical-slice music";
        return false;
    }
    return true;
}

bool ChiptuneVerticalSlice::register_cues(std::string* error) {
    for (std::size_t index = 0; index < cueSamples_.size(); ++index) {
        const auto request = cue_request(index);
        const auto song = audio::make_chiptune_sfx_song(request);
        auto rendered = audio::render_chiptune_audio_asset(
            song, std::max(0.35F, request.durationSeconds + 0.12F), error);
        if (rendered.samples.empty()) return false;
        audio::ResidentSampleDesc resident;
        resident.name = song.name + " [vertical slice cue]";
        resident.sampleRate = rendered.metadata.sampleRate;
        resident.channels = rendered.metadata.channels;
        resident.samples = std::move(rendered.samples);
        cueSamples_[index] = mixer_.register_resident_sample(std::move(resident), error);
        if (!cueSamples_[index]) return false;
    }
    return true;
}

void ChiptuneVerticalSlice::reset_world_state() {
    state_ = {};
    state_.health = 3;
    state_.player.size = {14.0F, 28.0F};
    state_.enemy.size = {16.0F, 28.0F};
    state_.camera.viewWidth = 320.0F;
    state_.camera.viewHeight = 180.0F;
    state_.camera.deadZoneHalf = {36.0F, 20.0F};
    camera_set_world_from_map(state_.camera, map_);

    TileVec2 playerSpawn{32.0F, 128.0F};
    TileVec2 enemySpawn{176.0F, 128.0F};
    TileVec2 collectible{232.0F, 120.0F};
    TileVec2 checkpoint{304.0F, 128.0F};
    for (const auto& layer : map_.objectLayers) {
        for (const auto& object : layer.objects) {
            if (object.type == "spawn") playerSpawn = object.position;
            else if (object.type == "enemy") enemySpawn = object.position;
            else if (object.type == "collectible") collectible = object.position;
            else if (object.type == "checkpoint") checkpoint = object.position;
        }
    }
    state_.player.min = playerSpawn;
    state_.checkpoint = playerSpawn;
    state_.enemy.min = enemySpawn;
    state_.enemyOrigin = enemySpawn;
    state_.collectible = collectible;
    state_.camera.center = state_.player.center();
    camera_follow(state_.camera, state_.player.center());
    hud_.set_interaction_prompt("Reach the checkpoint • Pulse Shot: Fire");
    jumpBufferSeconds_ = 0.0F;
    coyoteSeconds_ = 0.0F;
    jumpWasHeld_ = false;
    saved_.reset();
    (void)checkpoint;
}

void ChiptuneVerticalSlice::step(const ChiptuneSliceInput& input, float deltaSeconds) {
    if (!initialized_ || !finite(deltaSeconds) || deltaSeconds <= 0.0F) return;
    deltaSeconds = std::min(deltaSeconds, 0.05F);
    ++state_.frame;
    state_.damageCooldownSeconds = std::max(0.0F, state_.damageCooldownSeconds - deltaSeconds);

    if (input.restartPressed) {
        restart_from_checkpoint();
        return;
    }
    if (input.savePressed) save_game();
    if (input.loadPressed) (void)load_game();
    if (state_.finished) return;

    update_player(input, deltaSeconds);
    update_enemy(deltaSeconds);
    if (input.firePressed && !state_.projectile.active) {
        state_.projectile.active = true;
        state_.projectile.position = {state_.player.center().x, state_.player.min.y + 8.0F};
        state_.projectile.velocity = {state_.facingRight ? kProjectileSpeed : -kProjectileSpeed, 0.0F};
        play_cue(Cue::Shot);
        emit(ChiptuneSliceEventType::Shot);
    }
    update_projectile(deltaSeconds);
    handle_world_interactions();
    camera_follow(state_.camera, state_.player.center());

    if (state_.player.min.y > static_cast<float>(map_.pixel_height()) + 64.0F || state_.health <= 0)
        restart_from_checkpoint();
}

void ChiptuneVerticalSlice::run_replay(std::span<const ChiptuneSliceInput> inputs,
                                       float deltaSeconds) {
    for (const auto& input : inputs) step(input, deltaSeconds);
}

void ChiptuneVerticalSlice::update_player(const ChiptuneSliceInput& input, float deltaSeconds) {
    const float move = std::clamp(input.moveX, -1.0F, 1.0F);
    if (std::abs(move) > 0.01F) state_.facingRight = move > 0.0F;
    const float acceleration = state_.grounded ? kGroundAcceleration : kAirAcceleration;
    const float deceleration = state_.grounded ? kGroundDeceleration : kAirAcceleration;
    state_.velocity.x = move_towards(state_.velocity.x, move * kPlayerRunSpeed,
                                     (std::abs(move) > 0.01F ? acceleration : deceleration) * deltaSeconds);

    if (input.jumpPressed) jumpBufferSeconds_ = kJumpBufferWindow;
    else jumpBufferSeconds_ = std::max(0.0F, jumpBufferSeconds_ - deltaSeconds);
    coyoteSeconds_ = state_.grounded ? kCoyoteWindow : std::max(0.0F, coyoteSeconds_ - deltaSeconds);
    if (jumpBufferSeconds_ > 0.0F && coyoteSeconds_ > 0.0F) {
        state_.velocity.y = -kJumpSpeed;
        state_.grounded = false;
        jumpBufferSeconds_ = 0.0F;
        coyoteSeconds_ = 0.0F;
        play_cue(Cue::Jump);
        emit(ChiptuneSliceEventType::Jumped);
    }
    if (!input.jumpHeld && jumpWasHeld_ && state_.velocity.y < -80.0F) state_.velocity.y *= 0.48F;
    jumpWasHeld_ = input.jumpHeld;
    state_.velocity.y = std::min(state_.velocity.y + kGravity * deltaSeconds, kMaximumFallSpeed);

    const AabbSweep result = move_aabb(collisionGrid_, state_.player,
                                      {state_.velocity.x * deltaSeconds,
                                       state_.velocity.y * deltaSeconds});
    state_.player.min = result.position;
    if (result.hitLeft || result.hitRight) state_.velocity.x = 0.0F;
    if (result.hitTop || result.hitBottom) state_.velocity.y = 0.0F;
    state_.grounded = result.onGround || result.hitBottom;
}

void ChiptuneVerticalSlice::update_enemy(float deltaSeconds) {
    if (!state_.enemyAlive) return;
    float patrolRadius = 64.0F;
    for (const auto& layer : map_.objectLayers) for (const auto& object : layer.objects) {
        if (object.type == "enemy") {
            patrolRadius = object_property_float(object, "patrol_radius").value_or(patrolRadius);
            break;
        }
    }
    state_.enemy.min.x += state_.enemyDirection * kEnemySpeed * deltaSeconds;
    if (state_.enemy.min.x > state_.enemyOrigin.x + patrolRadius) {
        state_.enemy.min.x = state_.enemyOrigin.x + patrolRadius;
        state_.enemyDirection = -1.0F;
    } else if (state_.enemy.min.x < state_.enemyOrigin.x - patrolRadius) {
        state_.enemy.min.x = state_.enemyOrigin.x - patrolRadius;
        state_.enemyDirection = 1.0F;
    }
}

void ChiptuneVerticalSlice::update_projectile(float deltaSeconds) {
    if (!state_.projectile.active) return;
    state_.projectile.position.x += state_.projectile.velocity.x * deltaSeconds;
    state_.projectile.position.y += state_.projectile.velocity.y * deltaSeconds;
    const TileAabb projectileBox{{state_.projectile.position.x - 2.0F,
                                  state_.projectile.position.y - 2.0F}, {4.0F, 4.0F}};
    const AabbSweep collision = move_aabb(collisionGrid_, projectileBox,
                                          {state_.projectile.velocity.x * deltaSeconds, 0.0F});
    if (collision.hitLeft || collision.hitRight || collision.hitTop || collision.hitBottom ||
        state_.projectile.position.x < 0.0F ||
        state_.projectile.position.x > static_cast<float>(map_.pixel_width())) {
        state_.projectile.active = false;
        return;
    }
    if (state_.enemyAlive && overlaps(projectileBox, state_.enemy)) {
        state_.enemyAlive = false;
        state_.projectile.active = false;
        play_cue(Cue::EnemyHit, audio::AudioPriority::Hero);
        emit(ChiptuneSliceEventType::EnemyDefeated, "Patrol Bot");
    }
}

void ChiptuneVerticalSlice::handle_world_interactions() {
    const TileWorldQuery query = query_tile_world(map_, state_.player);
    if (query.totalDamagePerSecond > 0.0F && state_.damageCooldownSeconds <= 0.0F)
        damage_player("hazard tile");

    if (state_.enemyAlive && player_overlaps(state_.enemy) && state_.damageCooldownSeconds <= 0.0F)
        damage_player("Patrol Bot");

    const TileAabb collectibleBox{{state_.collectible.x - 7.0F, state_.collectible.y - 7.0F},
                                   {14.0F, 14.0F}};
    if (state_.collectibleActive && player_overlaps(collectibleBox)) {
        state_.collectibleActive = false;
        ++state_.tokens;
        play_cue(Cue::Coin, audio::AudioPriority::Hero);
        emit(ChiptuneSliceEventType::Collected, "Sun Token");
    }

    for (const auto& layer : map_.objectLayers) for (const auto& object : layer.objects) {
        if (object.type != "checkpoint" || !player_overlaps(object_bounds(object))) continue;
        if (!state_.checkpointActive) {
            state_.checkpointActive = true;
            state_.checkpoint = object.position;
            state_.checkpoint.y = std::min(state_.checkpoint.y, 128.0F);
            play_cue(Cue::Checkpoint, audio::AudioPriority::Hero);
            emit(ChiptuneSliceEventType::CheckpointActivated, object.name);
            save_game();
        }
        if (state_.tokens > 0 && !state_.finished) {
            state_.finished = true;
            emit(ChiptuneSliceEventType::Finished, "Original chiptune vertical slice complete");
        }
    }

    std::ostringstream prompt;
    prompt << "HP " << state_.health << "/3  •  Tokens " << state_.tokens;
    if (state_.finished) prompt << "  •  COMPLETE";
    else if (state_.checkpointActive) prompt << "  •  Checkpoint active";
    else prompt << "  •  Reach checkpoint";
    hud_.set_interaction_prompt(prompt.str());
}

void ChiptuneVerticalSlice::damage_player(std::string_view reason) {
    --state_.health;
    state_.damageCooldownSeconds = kDamageCooldown;
    state_.velocity.x = state_.facingRight ? -110.0F : 110.0F;
    state_.velocity.y = -170.0F;
    play_cue(Cue::Damage, audio::AudioPriority::Hero);
    emit(ChiptuneSliceEventType::Damaged, std::string(reason));
}

void ChiptuneVerticalSlice::emit(ChiptuneSliceEventType type, std::string detail) {
    events_.push_back({state_.frame, type, std::move(detail)});
}

void ChiptuneVerticalSlice::play_cue(Cue cue, audio::AudioPriority priority) {
    const auto sample = cueSamples_[static_cast<std::size_t>(cue)];
    if (!sample) return;
    audio::PlaySampleDesc play;
    play.sample = sample;
    play.bus = audio::AudioBusId::Effects;
    play.priority = priority;
    play.spatialized = false;
    play.gain = 0.9F;
    (void)mixer_.play_sample(play);
}

bool ChiptuneVerticalSlice::player_overlaps(const TileAabb& bounds) const noexcept {
    return overlaps(state_.player, bounds);
}

bool ChiptuneVerticalSlice::overlaps(const TileAabb& a, const TileAabb& b) noexcept {
    return a.min.x < b.max_x() && a.max_x() > b.min.x &&
           a.min.y < b.max_y() && a.max_y() > b.min.y;
}

std::optional<float> ChiptuneVerticalSlice::object_property_float(const TileObject& object,
                                                                   std::string_view name) noexcept {
    for (const auto& property : object.properties) {
        if (property.name != name) continue;
        try {
            std::size_t consumed{};
            const float value = std::stof(property.value, &consumed);
            if (consumed == property.value.size() && finite(value)) return value;
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::string ChiptuneVerticalSlice::hud_text() const {
    return hud_.interaction_prompt();
}

ChiptuneSliceSnapshot ChiptuneVerticalSlice::capture_snapshot() const noexcept {
    return {state_, deterministic_hash()};
}

bool ChiptuneVerticalSlice::restore_snapshot(const ChiptuneSliceSnapshot& snapshot) noexcept {
    if (!finite(snapshot.state.player.min.x) || !finite(snapshot.state.player.min.y) ||
        !finite(snapshot.state.velocity.x) || !finite(snapshot.state.velocity.y) ||
        snapshot.state.health < 0 || snapshot.state.health > 3) return false;
    state_ = snapshot.state;
    jumpBufferSeconds_ = 0.0F;
    coyoteSeconds_ = state_.grounded ? kCoyoteWindow : 0.0F;
    jumpWasHeld_ = false;
    return true;
}

void ChiptuneVerticalSlice::save_game() noexcept {
    saved_ = capture_snapshot();
    emit(ChiptuneSliceEventType::Saved);
}

bool ChiptuneVerticalSlice::load_game() noexcept {
    if (!saved_ || !restore_snapshot(*saved_)) return false;
    emit(ChiptuneSliceEventType::Loaded);
    return true;
}

void ChiptuneVerticalSlice::restart_from_checkpoint() {
    ++state_.deaths;
    state_.health = 3;
    state_.player.min = state_.checkpoint;
    state_.velocity = {};
    state_.grounded = false;
    state_.projectile.active = false;
    state_.damageCooldownSeconds = 0.75F;
    state_.finished = false;
    play_cue(Cue::Respawn, audio::AudioPriority::Hero);
    emit(ChiptuneSliceEventType::Respawned);
}

std::uint64_t ChiptuneVerticalSlice::deterministic_hash() const noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    hash_scalar(hash, state_.frame);
    hash_scalar(hash, state_.player.min.x); hash_scalar(hash, state_.player.min.y);
    hash_scalar(hash, state_.velocity.x); hash_scalar(hash, state_.velocity.y);
    hash_scalar(hash, state_.grounded); hash_scalar(hash, state_.facingRight);
    hash_scalar(hash, state_.health); hash_scalar(hash, state_.tokens); hash_scalar(hash, state_.deaths);
    hash_scalar(hash, state_.checkpoint.x); hash_scalar(hash, state_.checkpoint.y);
    hash_scalar(hash, state_.checkpointActive); hash_scalar(hash, state_.enemy.min.x);
    hash_scalar(hash, state_.enemy.min.y); hash_scalar(hash, state_.enemyDirection);
    hash_scalar(hash, state_.enemyAlive); hash_scalar(hash, state_.collectibleActive);
    hash_scalar(hash, state_.projectile.position.x); hash_scalar(hash, state_.projectile.position.y);
    hash_scalar(hash, state_.projectile.active); hash_scalar(hash, state_.finished);
    return hash;
}

std::string_view chiptune_slice_event_name(ChiptuneSliceEventType type) noexcept {
    switch (type) {
        case ChiptuneSliceEventType::MusicStarted: return "music_started";
        case ChiptuneSliceEventType::Jumped: return "jumped";
        case ChiptuneSliceEventType::Shot: return "shot";
        case ChiptuneSliceEventType::EnemyDefeated: return "enemy_defeated";
        case ChiptuneSliceEventType::Collected: return "collected";
        case ChiptuneSliceEventType::CheckpointActivated: return "checkpoint_activated";
        case ChiptuneSliceEventType::Damaged: return "damaged";
        case ChiptuneSliceEventType::Respawned: return "respawned";
        case ChiptuneSliceEventType::Saved: return "saved";
        case ChiptuneSliceEventType::Loaded: return "loaded";
        case ChiptuneSliceEventType::Finished: return "finished";
    }
    return "unknown";
}

} // namespace dve::gameplay
