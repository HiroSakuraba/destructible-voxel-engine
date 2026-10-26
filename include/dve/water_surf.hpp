#pragma once
#include "dve/water_hydrodynamics.hpp"
namespace dve {
struct SurfInput {float lean{},paddle{};};
struct SurfLoads {
 Float3 force{},torque{};
 float faceDrive{},railForce{},breakIntensity{},stability{};
 bool wipeout{};
};
// Pure, headless surfboard/rider controller. Returns rider energy input from
// paddling separately from downhill wave-face gravity and water-flow forces.
[[nodiscard]] SurfLoads evaluate_surfboard(const WaterHull& hull,
 const WaterRigidState& body,const OceanSample& water,
 const SurfInput& input,float immersion)noexcept;
}
