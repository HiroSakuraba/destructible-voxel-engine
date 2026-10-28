#pragma once
#include "dve/water_ocean.hpp"
namespace dve {
// Three-dimensional, headless water query. Surface elevation is always taken
// from the *existing* ocean; orbital wave motion decays with immersion depth.
enum class WaterClarity : unsigned { ClearOcean, Coastal, Turbid };
struct WaterVolumeSettings {
    float floorZ{-40.F},density{1025.F};
    WaterClarity clarity{WaterClarity::ClearOcean};
    bool customOptics{};
    Float3 absorptionRGB{.34F,.075F,.018F};
    Float3 scatteringRGB{.018F,.015F,.012F};
    Float3 diffuseKdRGB{.38F,.10F,.045F};
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
