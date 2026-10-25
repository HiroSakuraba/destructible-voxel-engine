#pragma once
#include "dve/water_volume.hpp"
#include "dve/water_hydrodynamics.hpp"
namespace dve {
struct SubmarineConfig {
    float dryMassKg{2350.F};
    float displacementM3{2.35F};
    float ballastCapacityKg{260.F},pumpRateKgPerSecond{45.F};
    float forwardThrustN{2200.F},verticalThrustN{3200.F},lateralThrustN{900.F};
    float yawTorqueNm{2000.F},pitchTorqueNm{900.F};
    float depthRatingM{80.F};
    float halfHeightM{1.F};
};
struct SubmarineState {
    float ballastKg{60.F},battery{1.F};
    bool depthHold{};
    float targetDepthM{10.F};
};
struct SubmarineInput {
    float forward{},strafe{},heave{},yaw{},pitch{},ballast{};
};
struct SubmarineLoads {
    Float3 forceNewton{},torqueNewtonMetres{};
    float submergedFraction{},depth{},pressurePascal{},batteryDraw{},verticalThrust{};
    bool ratedDepthExceeded{};
};
[[nodiscard]] bool valid_submarine_config(const SubmarineConfig&)noexcept;
// Evaluate controlled 6-DOF forces; updates ballast and energy on a fixed tick.
// Total vertical load includes buoyancy, thrusters and gravity: do not also
// pass this through the surface-vessel buoyancy step.
[[nodiscard]] SubmarineLoads evaluate_submarine(const SubmarineConfig&,
    SubmarineState&,const WaterRigidState&,const WaterVolumeSample&,
    const SubmarineInput&,float dt)noexcept;
}
