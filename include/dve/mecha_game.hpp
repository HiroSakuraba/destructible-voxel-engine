#pragma once
// M1-M6: portable Z-up mech gameplay; optional third-person controls and jump jets.
#include "dve/prototype_game.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
namespace dve {
struct MechMotorConfig {
    float maximumSpeed{11.0F}, acceleration{24.0F}, braking{28.0F};
    float boostSpeed{24.0F}, boostEnergyCost{32.0F}, energyRegeneration{16.0F};
    float radius{0.95F}, height{3.5F};
    float jetAcceleration{22.0F}, jetDrainPerSecond{32.0F};
    float maximumJetHeight{18.0F};
};
struct MechMotorState {
    Float3 position{}, velocity{};
    float legsYaw{}, torsoYaw{}, energy{100.0F};
};
struct MechMotorInput {
    Float3 move{}; // World XY direction; magnitude clamped to one.
    bool boost{};
};
[[nodiscard]] bool valid_mech_motor_config(const MechMotorConfig&) noexcept;
// Integrates velocity and energy. Caller must sweep the returned displacement before publishing position.
[[nodiscard]] Float3 integrate_mech_motor(MechMotorState&, const MechMotorConfig&,
                                           MechMotorInput, float dt) noexcept;
struct MechFootContact {
    Float3 ankle{},normal{0,0,1};
    std::uint64_t supportObject{};
    bool planted{},supported{};
};
// Gameplay events, NOT renderer-owned particles. The consumer chooses sound,
// decal and VFX presets, and may ignore events when culled.
struct MechFootstepEvent {
    Float3 position{},normal{0,0,1};
    std::uint64_t supportObject{};
    float intensity{};
    std::int32_t side{};
};
enum class MechCameraMode : std::uint8_t { Tactical, ThirdPerson };
enum class MechWeaponKind : std::uint8_t { Autocannon, Rocket };
struct MechProjectile {
    Float3 position{}, velocity{};
    float remainingSeconds{4.0F};
    std::uint64_t sequence{};
};
enum class MechEnemyKind : std::uint8_t { Dummy=0, Scout=1, Heavy=2, Turret=3 };
struct MechCombatTelemetry {
    std::uint64_t autocannonShots{}, rocketsFired{}, targetsDestroyed{}, impacts{};
    std::uint64_t voxelCarvesQueued{}, enemyShots{}, navigationReplans{};
    std::uint32_t activeRockets{};
    float energy{}, primaryCooldown{}, rocketCooldown{};
    float altitude{}, verticalSpeed{};
    bool jetActive{}, lockedOn{};
};
[[nodiscard]] bool scene_has_mecha(std::span<const PrototypeObjectView>);
[[nodiscard]] bool validate_mecha_scene(std::span<const PrototypeObjectView>,std::string* error=nullptr);
void register_mecha_components(ComponentTypeRegistry&);
class MechaGame {
public:
    explicit MechaGame(GameWorld&);
    ~MechaGame();
    MechaGame(const MechaGame&)=delete;
    MechaGame& operator=(const MechaGame&)=delete;
    [[nodiscard]] bool start(std::string* error=nullptr);
    void step(float dt);
    void look(float yawDelta,float pitchDelta);
    [[nodiscard]] bool after_restore(std::string_view state,std::string* error=nullptr);
    [[nodiscard]] std::string save_state() const;
    [[nodiscard]] const PrototypeHud& hud() const noexcept;
    [[nodiscard]] PrototypeCamera camera() const noexcept;
    [[nodiscard]] const MechMotorState& motor() const noexcept;
    [[nodiscard]] MechCameraMode camera_mode() const noexcept;
    [[nodiscard]] std::array<MechFootContact,2> foot_contacts() const noexcept;
    [[nodiscard]] std::span<const MechFootstepEvent> footstep_events() const noexcept;
    [[nodiscard]] std::string_view locked_target_key() const noexcept;
    [[nodiscard]] const MechCombatTelemetry& telemetry() const noexcept;
    [[nodiscard]] std::span<const RacingVisualEffect> impact_visuals() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dve
