#pragma once
#include "dve/water_ocean.hpp"
#include <array>
#include <cstdint>
namespace dve {
struct WaterHull {
    float massKg{480};
    float displacementCubicMetres{0.55F};
    float lengthMetres{4},beamMetres{1.8F},draftMetres{.8F};
    float linearDamping{80},angularDamping{160};
    std::uint32_t longitudinalProbes{3},lateralProbes{3};
};
struct WaterRigidState {
    Float3 position{};
    Quaternion orientation{};
    Float3 linearVelocity{},angularVelocity{}; // angular in world rad/s
};
struct WaterHydroLoads {
    Float3 forceNewton{},torqueNewtonMetres{};
    std::uint32_t immersedProbes{};
    float immersionFraction{},meanWaterHeight{},impactRate{};
};
[[nodiscard]] bool valid_water_hull(const WaterHull&)noexcept;
[[nodiscard]] WaterHydroLoads evaluate_water_hull(const OceanField&,double time,
    const WaterHull&,const WaterRigidState&,Float3 gravity={0,0,-9.81F})noexcept;
// Integrates a light-weight 6-DOF hull in the portable reference game controller.
void step_water_hull(WaterRigidState&,const WaterHull&,const WaterHydroLoads&,
    float fixedSeconds,Float3 extraForce={},Float3 extraTorque={})noexcept;
}
