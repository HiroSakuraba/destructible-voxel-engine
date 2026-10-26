#pragma once
#include "dve/water_ocean.hpp"
namespace dve {
// Three-dimensional, headless water query. Surface elevation is always taken
// from the *existing* ocean; orbital wave motion decays with immersion depth.
enum class WaterClarity : unsigned { ClearOcean, Coastal, Turbid };
struct WaterVolumeSettings {
    float floorZ{-40.F},density{1025.F};
    WaterClarity clarity{WaterClarity::ClearOcean};
};
struct WaterVolumeSample {
    float surfaceHeight{},seafloorHeight{},depthBelowSurface{};
    float pressurePascal{101325.F},density{1025.F};
    Float3 flowVelocity{};
    // Initial linear-light optical interface, in inverse metres; S2 renders it.
    Float3 absorptionRGB{},scatteringRGB{},diffuseKdRGB{};
    float turbidity{},particleDensity{};
    bool submerged{};
};
[[nodiscard]] bool valid_water_volume_settings(const WaterVolumeSettings&)noexcept;
[[nodiscard]] WaterVolumeSample sample_water_volume(
    const OceanField&,const WaterVolumeSettings&,Float3 worldPosition,double seconds)noexcept;
}
