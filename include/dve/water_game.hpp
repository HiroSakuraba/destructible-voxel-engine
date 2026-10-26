#pragma once
#include "dve/prototype_game.hpp"
#include "dve/water_hydrodynamics.hpp"
#include "dve/water_vessel_dynamics.hpp"
#include "dve/submarine_dynamics.hpp"
#include "dve/water_surf.hpp"
#include <memory>
namespace dve {
[[nodiscard]] bool scene_has_water(std::span<const PrototypeObjectView>);
[[nodiscard]] bool validate_water_scene(std::span<const PrototypeObjectView>,std::string* error=nullptr);
void register_water_components(ComponentTypeRegistry&);
struct WaterGameTelemetry {
    float speed{},waterHeight{},immersion{},rollRadians{},pitchRadians{};
    float impactRate{},rudder{},throttle{};
    float apparentWind{},sailDrive{},planingLift{},lateralSlip{},sailTrim{};
    float localFoam{},wakeHeight{};unsigned activeWakes{};
    float surfBreaking{},surfStability{},surfFaceDrive{};unsigned surfWipeouts{};
    WatercraftType type{WatercraftType::Dinghy};
    float subDepth{},subTargetDepth{},subBallastKg{},subBattery{},subPressurePa{};
    bool subDepthHold{},subOverDepth{};
    unsigned activeVessel{},vesselCount{};
};
class WaterGame {
public:
    explicit WaterGame(GameWorld&);
    ~WaterGame();
    [[nodiscard]] bool start(std::string* error=nullptr);
    void step(float dt);
    [[nodiscard]] bool after_restore(std::string* error=nullptr);
    [[nodiscard]] const PrototypeHud& hud()const noexcept;
    [[nodiscard]] PrototypeCamera camera()const noexcept;
    [[nodiscard]] const OceanField& ocean()const noexcept;
    [[nodiscard]] double ocean_time()const noexcept;
    [[nodiscard]] std::span<const WaterSprayEffect> spray_effects()const noexcept;
    [[nodiscard]] const WaterGameTelemetry& telemetry()const noexcept;
private:struct Impl;std::unique_ptr<Impl> impl_;
};
}
