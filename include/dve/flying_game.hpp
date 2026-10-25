#pragma once
// Portable Z-up, +Y-forward six-form flight prototype.
// Flight is runtime-owned and uses GameWorld collision sweeps rather than requiring Jolt.
#include "dve/prototype_game.hpp"
#include <cstdint>
#include <span>
namespace dve {
enum class FlightMode : std::uint8_t { Float, Glider, Biplane, Jet, Hover, Bird };
enum class FlightAssist : std::uint8_t { Assisted, Simulation };
struct FlightProfile {
    const char* name{};
    float wingArea{}, liftSlope{}, liftLimit{}, stallAngle{}, parasiteDrag{}, inducedDrag{};
    float thrust{}, buoyancyFraction{}, bodyDrag{}, response{}, transitionSeconds{};
};
[[nodiscard]] const FlightProfile& flight_profile(FlightMode) noexcept;
[[nodiscard]] const char* flight_mode_name(FlightMode) noexcept;
[[nodiscard]] bool scene_has_flying(std::span<const PrototypeObjectView>);
[[nodiscard]] bool validate_flying_scene(std::span<const PrototypeObjectView>,std::string* error=nullptr);
void register_flying_components(ComponentTypeRegistry&);
struct FlightTelemetry {
    float airspeed{},groundspeed{},verticalSpeed{},altitude{},angleOfAttack{},liftCoefficient{},dragCoefficient{};
    float fuel{1},battery{1},stamina{1},spool{},dynamicPressure{},morph{};
    float windSpeed{},stallMargin{};
    unsigned nextGate{},gateCount{};
    bool stalled{},transitioning{},crashed{};
    FlightMode mode{FlightMode::Float},target{FlightMode::Float};
};
class FlyingGame {
public:
    explicit FlyingGame(GameWorld&);
    ~FlyingGame();
    [[nodiscard]] bool start(std::string* error=nullptr);
    void step(float dt);
    [[nodiscard]] bool after_restore(std::string* error=nullptr);
    [[nodiscard]] const PrototypeHud& hud()const noexcept;
    [[nodiscard]] PrototypeCamera camera()const noexcept;
    [[nodiscard]] const FlightTelemetry& telemetry()const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
