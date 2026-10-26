#pragma once
#include "dve/transform.hpp"
#include <array>
#include <cstdint>
#include <span>
namespace dve {
// Ocean queries are deterministic pure functions of position and fixed simulation time.
// Z is up, metres/seconds throughout. Renderers and hull probes use this same evaluator.
enum class OceanPreset : std::uint8_t { Calm=0, Breeze=1, Storm=2 };
struct OceanWave {
    Float2 direction{0,1}; // horizontal unit direction
    float amplitude{},wavelength{20},phase{},steepness{0.45F};
};
struct OceanSample {
    float height{};
    Float3 normal{0,0,1};
    Float3 waterVelocity{};
    float foam{}; // dimensionless crest steepness estimate [0,1]
};
class OceanField {
public:
    [[nodiscard]] bool set_preset(OceanPreset preset,Float3 current={}) noexcept;
    [[nodiscard]] bool set_waves(std::span<const OceanWave> waves,Float3 current={}) noexcept;
    [[nodiscard]] OceanSample sample(float x,float y,double seconds)const noexcept;
    [[nodiscard]] OceanPreset preset()const noexcept{return preset_;}
    [[nodiscard]] Float3 current()const noexcept{return current_;}
    [[nodiscard]] std::span<const OceanWave> waves()const noexcept{return {waves_.data(),count_};}
    [[nodiscard]] float maximum_wave_height()const noexcept{return maxHeight_;}
private:
    std::array<OceanWave,16> waves_{};
    std::uint32_t count_{};
    Float3 current_{};
    float maxHeight_{};
    OceanPreset preset_{OceanPreset::Calm};
};
[[nodiscard]] bool valid_ocean_wave(const OceanWave&)noexcept;
}
