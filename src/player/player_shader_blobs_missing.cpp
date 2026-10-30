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
std::span<const std::byte> player_rc_cascade_ps() noexcept { return {}; }
std::span<const std::byte> player_rc_gather_ps() noexcept { return {}; }
std::span<const std::byte> player_gi_temporal_ps() noexcept { return {}; }
std::span<const std::byte> player_gi_upsample_ps() noexcept { return {}; }
std::span<const std::byte> player_split_rc_surface_cs() noexcept { return {}; }
std::span<const std::byte> player_split_rc_trace_cs() noexcept { return {}; }
std::span<const std::byte> player_split_rc_upsample_cs() noexcept { return {}; }
std::span<const std::byte> player_split_rc_clear_cs() noexcept { return {}; }
std::span<const std::byte> player_split_rc_prepare_cs() noexcept { return {}; }
std::span<const std::byte> player_split_rc_weighted_merge_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_probe_assign_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_intervals_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_compact_offsets_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_build_queries_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_accumulate_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_resolve_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_spatial_merge_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_scatter_cs() noexcept { return {}; }
std::span<const std::byte> split_rc_diffuse_gather_cs() noexcept { return {}; }
} // namespace dve::player::shader_blobs
