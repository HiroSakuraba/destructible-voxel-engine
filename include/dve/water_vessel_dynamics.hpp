#pragma once
#include "dve/water_hydrodynamics.hpp"
#include <cstdint>
namespace dve {
// Distinct vessel hydrodynamics over one shared OceanField, independent of
// editor, player and any specific rigid-body solver.
enum class WatercraftType : std::uint8_t {
    Dinghy=0, Sailboat=1, PirateShip=2, Speedboat=3, WaveRunner=4, MiniSub=6
};
struct VesselDynamics {
    WatercraftType type{WatercraftType::Dinghy};
    float sailArea{0};      // m^2
    float keelArea{0};      // m^2
    float mastHeight{0};    // m above the center of mass
    float thrustNewtons{1400};
    float rudderNewtons{380}; // W1 compatibility: max rudder torque reference
    float planingFactor{0}; // dimensionless effective planing lift coefficient
};
struct VesselInput {
    float throttle{}; // [-1,1], engine throttle or sail exposure
    float helm{};     // [-1,1]
    float sailTrim{0.6F}; // signed sail angle in radians to craft-forward
};
struct VesselDriveLoads {
    Float3 force{},torque{};
    float apparentWind{},forwardWaterSpeed{},lateralWaterSpeed{};
    float sailThrust{},planingLift{},keelForce{},rudderMoment{};
};
// Pure forces at actual wind/water velocities and finite impulse moments.
[[nodiscard]] bool valid_vessel_dynamics(const VesselDynamics&)noexcept;
[[nodiscard]] VesselDriveLoads evaluate_vessel_dynamics(
    const VesselDynamics&,const WaterHull&,const WaterRigidState&,
    const OceanSample&,Float3 windVelocity,const VesselInput& input,
    float immersionFraction)noexcept;
[[nodiscard]] const char* watercraft_name(WatercraftType)noexcept;
}
