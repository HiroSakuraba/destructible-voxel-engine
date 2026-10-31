#pragma once
// One and only one runtime diffuse indirect-light provider per frame.
// The material shader REPLACES environment diffuse with traced GI when the
// traced texture's alpha says it is valid. SHE/split-sum own specular only.
#include <cstdint>
namespace dve::player {
enum class IndirectDiffuseOwner : std::uint8_t {
    EnvironmentIbl,
    TracedOneBounce,
    ScreenRadianceCascades,
    SplitRadianceCascades
};
[[nodiscard]] constexpr IndirectDiffuseOwner choose_indirect_diffuse_owner(
    bool tracedLightingEnabled, bool requestCascades,
    bool cascadesAvailable, bool experimentalSplitEnabled) noexcept {
    if(!tracedLightingEnabled)return IndirectDiffuseOwner::EnvironmentIbl;
    if(!requestCascades || !cascadesAvailable)return IndirectDiffuseOwner::TracedOneBounce;
    if(experimentalSplitEnabled)return IndirectDiffuseOwner::SplitRadianceCascades;
    return IndirectDiffuseOwner::ScreenRadianceCascades;
}
[[nodiscard]] constexpr bool uses_traced_diffuse(IndirectDiffuseOwner owner) noexcept {
    return owner!=IndirectDiffuseOwner::EnvironmentIbl;
}
[[nodiscard]] constexpr bool uses_screen_cascades(IndirectDiffuseOwner owner) noexcept {
    return owner==IndirectDiffuseOwner::ScreenRadianceCascades;
}
[[nodiscard]] constexpr bool uses_split_cascades(IndirectDiffuseOwner owner) noexcept {
    return owner==IndirectDiffuseOwner::SplitRadianceCascades;
}
} // namespace dve::player
