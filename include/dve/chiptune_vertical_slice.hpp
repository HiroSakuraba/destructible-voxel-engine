#pragma once

#include "dve/audio/chiptune.hpp"
#include "dve/audio/mixer.hpp"
#include "dve/game_ui.hpp"
#include "dve/tilemap2d.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dve::gameplay {

struct ChiptuneSliceInput {
    float moveX{};
    bool jumpPressed{};
    bool jumpHeld{};
    bool firePressed{};
    bool savePressed{};
    bool loadPressed{};
    bool restartPressed{};
};

enum class ChiptuneSliceEventType : std::uint8_t {
    MusicStarted,
    Jumped,
    Shot,
    EnemyDefeated,
    Collected,
    CheckpointActivated,
    Damaged,
    Respawned,
    Saved,
    Loaded,
    Finished
};

struct ChiptuneSliceEvent {
    std::uint64_t frame{};
    ChiptuneSliceEventType type{};
    std::string detail;
};

struct ChiptuneSliceProjectile {
    TileVec2 position{};
    TileVec2 velocity{};
    bool active{};
};

struct ChiptuneSliceState {
    std::uint64_t frame{};
    TileAabb player{};
    TileVec2 velocity{};
    bool grounded{};
    bool facingRight{true};
    int health{3};
    int tokens{};
    int deaths{};
    TileVec2 checkpoint{};
    bool checkpointActive{};
    TileAabb enemy{};
    TileVec2 enemyOrigin{};
    float enemyDirection{1.0F};
    bool enemyAlive{true};
    bool collectibleActive{true};
    TileVec2 collectible{};
    ChiptuneSliceProjectile projectile{};
    Camera2D camera{};
    bool finished{};
    float damageCooldownSeconds{};
};

struct ChiptuneSliceSnapshot {
    ChiptuneSliceState state{};
    std::uint64_t stateHash{};
};

struct ChiptuneSliceAssets {
    std::filesystem::path level{"assets/tilemaps/v213_original_level.dvetilemap"};
    std::filesystem::path music{"assets/chiptune/v217_sun_route_theme.dvechip"};
};

class ChiptuneVerticalSlice {
public:
    explicit ChiptuneVerticalSlice(audio::AudioMixer& mixer);

    [[nodiscard]] bool initialize(const std::filesystem::path& projectRoot,
                                  ChiptuneSliceAssets assets = {},
                                  std::string* error = nullptr);
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    void step(const ChiptuneSliceInput& input, float deltaSeconds);
    void run_replay(std::span<const ChiptuneSliceInput> inputs, float deltaSeconds);

    [[nodiscard]] const ChiptuneSliceState& state() const noexcept { return state_; }
    [[nodiscard]] const TileMap& map() const noexcept { return map_; }
    [[nodiscard]] const CollisionGrid& collision_grid() const noexcept { return collisionGrid_; }
    [[nodiscard]] const ui::GameHudModel& hud() const noexcept { return hud_; }
    [[nodiscard]] std::string hud_text() const;
    [[nodiscard]] const std::vector<ChiptuneSliceEvent>& events() const noexcept { return events_; }
    void clear_events() noexcept { events_.clear(); }

    [[nodiscard]] ChiptuneSliceSnapshot capture_snapshot() const noexcept;
    [[nodiscard]] bool restore_snapshot(const ChiptuneSliceSnapshot& snapshot) noexcept;
    void save_game() noexcept;
    [[nodiscard]] bool load_game() noexcept;
    void restart_from_checkpoint();

    [[nodiscard]] std::uint64_t deterministic_hash() const noexcept;
    [[nodiscard]] audio::AudioSourceHandle music_source() const noexcept { return musicSource_; }
    [[nodiscard]] audio::AudioBusId music_bus() const noexcept { return audio::AudioBusId::Music; }
    [[nodiscard]] audio::AudioBusId effects_bus() const noexcept { return audio::AudioBusId::Effects; }

private:
    enum class Cue : std::uint8_t { Jump, Shot, EnemyHit, Coin, Checkpoint, Damage, Respawn, Count };

    [[nodiscard]] bool load_level(const std::filesystem::path& path, std::string* error);
    [[nodiscard]] bool load_audio(const std::filesystem::path& path, std::string* error);
    [[nodiscard]] bool register_cues(std::string* error);
    void reset_world_state();
    void update_player(const ChiptuneSliceInput& input, float deltaSeconds);
    void update_enemy(float deltaSeconds);
    void update_projectile(float deltaSeconds);
    void handle_world_interactions();
    void damage_player(std::string_view reason);
    void emit(ChiptuneSliceEventType type, std::string detail = {});
    void play_cue(Cue cue, audio::AudioPriority priority = audio::AudioPriority::Important);
    [[nodiscard]] bool player_overlaps(const TileAabb& bounds) const noexcept;
    [[nodiscard]] static bool overlaps(const TileAabb& a, const TileAabb& b) noexcept;
    [[nodiscard]] static std::optional<float> object_property_float(const TileObject& object,
                                                                    std::string_view name) noexcept;

    audio::AudioMixer& mixer_;
    ChiptuneSliceAssets assetPaths_{};
    TileMap map_{};
    CollisionGrid collisionGrid_{};
    ChiptuneSliceState state_{};
    ui::GameHudModel hud_{};
    std::vector<ChiptuneSliceEvent> events_{};
    std::optional<ChiptuneSliceSnapshot> saved_{};
    audio::SampleId musicSample_{};
    audio::AudioSourceHandle musicSource_{};
    std::array<audio::SampleId, static_cast<std::size_t>(Cue::Count)> cueSamples_{};
    float jumpBufferSeconds_{};
    float coyoteSeconds_{};
    bool jumpWasHeld_{};
    bool initialized_{};
};

[[nodiscard]] std::string_view chiptune_slice_event_name(ChiptuneSliceEventType type) noexcept;

} // namespace dve::gameplay
