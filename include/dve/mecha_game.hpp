#pragma once
// M1/M2: portable Z-up top-down mech movement and combat; independent of Jolt and Lua.
#include "dve/prototype_game.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
namespace dve {
struct MechMotorConfig {
    float maximumSpeed{11.0F}, acceleration{24.0F}, braking{28.0F};
    float boostSpeed{24.0F}, boostEnergyCost{32.0F}, energyRegeneration{16.0F};
    float radius{0.95F}, height{3.5F};
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
enum class MechWeaponKind : std::uint8_t { Autocannon, Rocket };
struct MechProjectile {
    Float3 position{}, velocity{};
    float remainingSeconds{4.0F};
    std::uint64_t sequence{};
};
struct MechCombatTelemetry {
    std::uint64_t autocannonShots{}, rocketsFired{}, targetsDestroyed{}, impacts{};
    std::uint32_t activeRockets{};
    float energy{}, primaryCooldown{}, rocketCooldown{};
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
    [[nodiscard]] bool after_restore(std::string* error=nullptr);
    [[nodiscard]] const PrototypeHud& hud() const noexcept;
    [[nodiscard]] PrototypeCamera camera() const noexcept;
    [[nodiscard]] const MechMotorState& motor() const noexcept;
    [[nodiscard]] const MechCombatTelemetry& telemetry() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dve
