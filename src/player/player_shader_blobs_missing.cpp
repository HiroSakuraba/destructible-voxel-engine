#include "player_shader_blobs.hpp"
// Linked when the build had no DXC with SPIR-V output: the Vulkan player renderer reports
// itself unavailable instead of failing at pipeline creation.
namespace dve::player::shader_blobs {
bool available() noexcept { return false; }
std::span<const std::byte> live_ibl_material_vs() noexcept { return {}; }
std::span<const std::byte> live_ibl_material_ps() noexcept { return {}; }
std::span<const std::byte> live_environment_skybox_vs() noexcept { return {}; }
std::span<const std::byte> live_environment_skybox_ps() noexcept { return {}; }
std::span<const std::byte> live_csm_caster_vs() noexcept { return {}; }
std::span<const std::byte> live_csm_caster_ps() noexcept { return {}; }
std::span<const std::byte> player_present_vs() noexcept { return {}; }
std::span<const std::byte> player_present_ps() noexcept { return {}; }
std::span<const std::byte> player_ssao_ps() noexcept { return {}; }
std::span<const std::byte> player_traced_ao_ps() noexcept { return {}; }
std::span<const std::byte> player_traced_gi_ps() noexcept { return {}; }
} // namespace dve::player::shader_blobs
