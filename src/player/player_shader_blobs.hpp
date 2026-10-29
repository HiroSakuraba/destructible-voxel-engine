#pragma once
// SPIR-V for the Vulkan player renderer, compiled by DXC at build time and embedded by
// cmake/DveEmbedSpirv.cmake (dve_player_shader_blobs.cpp). Without DXC the build links
// player_shader_blobs_missing.cpp instead: available() is false and every span is empty.
#include <cstddef>
#include <span>

namespace dve::player::shader_blobs {
[[nodiscard]] bool available() noexcept;
[[nodiscard]] std::span<const std::byte> live_ibl_material_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_ibl_material_ps() noexcept;
[[nodiscard]] std::span<const std::byte> live_environment_skybox_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_environment_skybox_ps() noexcept;
[[nodiscard]] std::span<const std::byte> live_csm_caster_vs() noexcept;
[[nodiscard]] std::span<const std::byte> live_csm_caster_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_present_vs() noexcept;
[[nodiscard]] std::span<const std::byte> player_present_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_ssao_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_traced_ao_ps() noexcept;
[[nodiscard]] std::span<const std::byte> player_traced_gi_ps() noexcept;
} // namespace dve::player::shader_blobs
